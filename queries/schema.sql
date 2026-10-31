BEGIN;

CREATE TABLE IF NOT EXISTS sites (
  id        SERIAL PRIMARY KEY,
  name      TEXT NOT NULL,
  location  TEXT,
  is_active BOOLEAN NOT NULL DEFAULT TRUE
);
ALTER TABLE sites ADD COLUMN IF NOT EXISTS is_active BOOLEAN NOT NULL DEFAULT TRUE;
CREATE UNIQUE INDEX IF NOT EXISTS sites_name_key ON sites (name);

CREATE TABLE IF NOT EXISTS users (
  id            SERIAL PRIMARY KEY,
  name          TEXT NOT NULL,
  username      TEXT NOT NULL UNIQUE,
  password_hash TEXT NOT NULL,
  role          TEXT NOT NULL CHECK (role IN ('admin','supervisor')),
  is_active     BOOLEAN NOT NULL DEFAULT TRUE,
  created_at    TIMESTAMPTZ NOT NULL DEFAULT now()
);

CREATE TABLE IF NOT EXISTS user_sites (
  user_id INT NOT NULL REFERENCES users(id),
  site_id INT NOT NULL REFERENCES sites(id),
  PRIMARY KEY (user_id, site_id)
);

CREATE TABLE IF NOT EXISTS materials (
  id        SERIAL PRIMARY KEY,
  name      TEXT NOT NULL UNIQUE,
  unit      TEXT NOT NULL,
  is_active BOOLEAN NOT NULL DEFAULT TRUE
);

CREATE TABLE IF NOT EXISTS stock_txns (
  id           BIGSERIAL PRIMARY KEY,
  site_id      INT NOT NULL REFERENCES sites(id),
  material_id  INT NOT NULL REFERENCES materials(id),
  type         TEXT NOT NULL,
  quantity     NUMERIC(14,3) NOT NULL CHECK (quantity>0),
  txn_date     DATE NOT NULL DEFAULT ((now() AT TIME ZONE 'Asia/Kolkata')::date),
  note         TEXT,
  party        TEXT,
  transfer_ref UUID,
  created_by   INT NOT NULL REFERENCES users(id),
  created_at   TIMESTAMPTZ NOT NULL DEFAULT now(),
  updated_at   TIMESTAMPTZ
);
ALTER TABLE stock_txns ADD COLUMN IF NOT EXISTS party TEXT;

-- duplicate-tap guard: the same UUID can only be saved once
ALTER TABLE stock_txns ADD COLUMN IF NOT EXISTS client_ref UUID;
CREATE UNIQUE INDEX IF NOT EXISTS stock_txns_client_ref_key ON stock_txns (client_ref);

ALTER TABLE stock_txns DROP CONSTRAINT IF EXISTS stock_txns_type_check;
ALTER TABLE stock_txns DROP CONSTRAINT IF EXISTS stock_txns_quantity_check;
ALTER TABLE stock_txns ADD CONSTRAINT stock_txns_quantity_check CHECK (quantity>0);

ALTER TABLE stock_txns ADD CONSTRAINT stock_txns_type_check CHECK (type IN
  ('IN','USAGE','ADMIN_PLUS','ADMIN_MINUS','TRANSFER_IN','TRANSFER_OUT',
   'BORROW_IN','BORROW_RETURN','LEND_OUT','LEND_RETURN','REJECT','REPROCESS_IN'));

CREATE INDEX IF NOT EXISTS stock_txns_lookup_idx ON stock_txns (site_id, material_id, txn_date);

CREATE TABLE IF NOT EXISTS reorder_settings (
  site_id        INT NOT NULL REFERENCES sites(id),
  material_id    INT NOT NULL REFERENCES materials(id),
  lead_time_days INT NOT NULL CHECK (lead_time_days BETWEEN 0 AND 365),
  safety_days    INT NOT NULL CHECK (safety_days    BETWEEN 0 AND 365),
  updated_at     TIMESTAMPTZ NOT NULL DEFAULT now(),
  PRIMARY KEY (site_id, material_id)
);

CREATE TABLE IF NOT EXISTS audit_log (
  id         BIGSERIAL PRIMARY KEY,
  table_name TEXT NOT NULL,
  record_id  BIGINT,
  action     TEXT NOT NULL,       -- UPDATE, DELETE, LOGIN, LOGIN_FAILED, USER_CREATED...
  old_data   JSONB,
  new_data   JSONB,
  changed_by INT REFERENCES users(id),
  changed_at TIMESTAMPTZ NOT NULL DEFAULT now()
);

CREATE TABLE IF NOT EXISTS transfer_requests (
  id            BIGSERIAL PRIMARY KEY,
  from_site_id  INT NOT NULL REFERENCES sites(id),
  to_site_id    INT NOT NULL REFERENCES sites(id),
  material_id   INT NOT NULL REFERENCES materials(id),
  quantity      NUMERIC(14,3) NOT NULL CHECK (quantity>0),
  note          TEXT,
  status        TEXT NOT NULL DEFAULT 'PENDING' CHECK (status IN ('PENDING','APPROVED','REJECTED','CANCELLED')),
  requested_by  INT NOT NULL REFERENCES users(id),
  requested_at  TIMESTAMPTZ NOT NULL DEFAULT now(),
  decided_by    INT REFERENCES users(id),
  decided_at    TIMESTAMPTZ,
  reject_reason TEXT,
  transfer_ref  UUID,
  CHECK (from_site_id <> to_site_id)
);
CREATE INDEX IF NOT EXISTS transfer_requests_status_idx ON transfer_requests (status);

CREATE TABLE IF NOT EXISTS user_requests (
  id            BIGSERIAL PRIMARY KEY,
  name          TEXT NOT NULL,
  username      TEXT NOT NULL,
  password_hash TEXT NOT NULL,
  site_id       INT NOT NULL REFERENCES sites(id),
  status        TEXT NOT NULL DEFAULT 'PENDING' CHECK (status IN ('PENDING','APPROVED','REJECTED')),
  requested_at  TIMESTAMPTZ NOT NULL DEFAULT now(),
  decided_by    INT REFERENCES users(id),
  decided_at    TIMESTAMPTZ,
  reject_reason TEXT
);
CREATE INDEX IF NOT EXISTS user_requests_status_idx ON user_requests (status);
CREATE UNIQUE INDEX IF NOT EXISTS user_requests_one_pending
  ON user_requests (username) WHERE status = 'PENDING';

-- =====================================================================
-- PALLET MODULE
--   * Wood is a normal material (unit = 'CFT').
--   * Each pallet size is one pallet type, with its own CFT factor.
--   * CFT factor is versioned (append-only history) so old production
--     stays correct when the factor changes.
-- =====================================================================
CREATE TABLE IF NOT EXISTS pallet_types (
  id         SERIAL PRIMARY KEY,
  name       TEXT NOT NULL UNIQUE,
  size       TEXT,
  is_active  BOOLEAN NOT NULL DEFAULT TRUE,
  created_at TIMESTAMPTZ NOT NULL DEFAULT now()
);
ALTER TABLE pallet_types ADD COLUMN IF NOT EXISTS size TEXT;

-- Append-only history. Never UPDATE an old row; INSERT a new version.
CREATE TABLE IF NOT EXISTS pallet_type_cft (
  id             BIGSERIAL PRIMARY KEY,
  pallet_type_id INT NOT NULL REFERENCES pallet_types(id),
  cft_per_pallet NUMERIC(10,4) NOT NULL CHECK (cft_per_pallet > 0),
  effective_from DATE NOT NULL DEFAULT ((now() AT TIME ZONE 'Asia/Kolkata')::date),
  note           TEXT,
  created_by     INT NOT NULL REFERENCES users(id),
  created_at     TIMESTAMPTZ NOT NULL DEFAULT now()
);
-- (older databases had a UNIQUE rule here that blocked same-day corrections)
ALTER TABLE pallet_type_cft DROP CONSTRAINT IF EXISTS pallet_type_cft_pallet_type_id_effective_from_key;
CREATE INDEX IF NOT EXISTS pallet_type_cft_lookup
  ON pallet_type_cft (pallet_type_id, effective_from DESC);

-- The CFT factor in force for a given pallet type on a given date.
CREATE OR REPLACE FUNCTION cft_for(p_type INT, p_date DATE)
RETURNS NUMERIC AS $$
  SELECT cft_per_pallet
  FROM pallet_type_cft
  WHERE pallet_type_id = p_type AND effective_from <= p_date
  ORDER BY effective_from DESC, id DESC
  LIMIT 1;
$$ LANGUAGE sql STABLE;

-- One row = "on this day, at this site, we built N pallets of type T, using M CFT of wood".
-- cft_per_pallet and cft_consumed are saved copies: changing the factor later never rewrites history.
CREATE TABLE IF NOT EXISTS pallet_production (
  id               BIGSERIAL PRIMARY KEY,
  site_id          INT NOT NULL REFERENCES sites(id),
  pallet_type_id   INT NOT NULL REFERENCES pallet_types(id),
  quantity         INT NOT NULL CHECK (quantity > 0),
  cft_per_pallet   NUMERIC(10,4) NOT NULL CHECK (cft_per_pallet > 0),
  cft_consumed     NUMERIC(14,3) NOT NULL CHECK (cft_consumed > 0),
  wood_material_id INT NOT NULL REFERENCES materials(id),
  txn_date         DATE NOT NULL DEFAULT ((now() AT TIME ZONE 'Asia/Kolkata')::date),
  note             TEXT,
  client_ref       UUID,
  created_by       INT NOT NULL REFERENCES users(id),
  created_at       TIMESTAMPTZ NOT NULL DEFAULT now(),
  updated_at       TIMESTAMPTZ
);

-- wood rows created by a production run point back to it (so they are locked)
ALTER TABLE stock_txns ADD COLUMN IF NOT EXISTS production_id BIGINT REFERENCES pallet_production(id);

CREATE TABLE IF NOT EXISTS pallet_production_reversals (
  production_id BIGINT PRIMARY KEY REFERENCES pallet_production(id),
  reversed_by   INT NOT NULL REFERENCES users(id),
  reversed_at   TIMESTAMPTZ NOT NULL DEFAULT now(),
  reason        TEXT NOT NULL
);

CREATE UNIQUE INDEX IF NOT EXISTS pallet_production_client_ref_key
  ON pallet_production (client_ref);
CREATE INDEX IF NOT EXISTS pallet_production_lookup
  ON pallet_production (site_id, pallet_type_id, txn_date);

-- Pallet ledger: purchases, production, sales/issues, corrections.
CREATE TABLE IF NOT EXISTS pallet_txns (
  id             BIGSERIAL PRIMARY KEY,
  site_id        INT NOT NULL REFERENCES sites(id),
  pallet_type_id INT NOT NULL REFERENCES pallet_types(id),
  type           TEXT NOT NULL CHECK (type IN ('IN','PRODUCED','OUT','ADJUST_PLUS','ADJUST_MINUS')),
  quantity       INT NOT NULL CHECK (quantity > 0),
  party          TEXT,
  note           TEXT,
  txn_date       DATE NOT NULL DEFAULT ((now() AT TIME ZONE 'Asia/Kolkata')::date),
  production_id  BIGINT REFERENCES pallet_production(id),
  client_ref     UUID,
  created_by     INT NOT NULL REFERENCES users(id),
  created_at     TIMESTAMPTZ NOT NULL DEFAULT now()
);
CREATE UNIQUE INDEX IF NOT EXISTS pallet_txns_client_ref_key
  ON pallet_txns (client_ref);
CREATE INDEX IF NOT EXISTS pallet_txns_lookup
  ON pallet_txns (site_id, pallet_type_id, txn_date);

CREATE OR REPLACE FUNCTION txn_sign(t TEXT) RETURNS INT AS $$
  SELECT CASE WHEN t IN ('IN','ADMIN_PLUS','TRANSFER_IN','BORROW_IN','LEND_RETURN','REPROCESS_IN') THEN 1 ELSE -1 END;
$$ LANGUAGE sql IMMUTABLE;

CREATE OR REPLACE FUNCTION pallet_sign(t TEXT) RETURNS INT AS $$
  SELECT CASE WHEN t IN ('IN','PRODUCED','ADJUST_PLUS') THEN 1 ELSE -1 END;
$$ LANGUAGE sql IMMUTABLE;

-- only site+material pairs that already have at least one entry
CREATE OR REPLACE VIEW current_stock AS
SELECT site_id, material_id, SUM(txn_sign(type) * quantity) AS balance
FROM stock_txns GROUP BY site_id, material_id;

-- EVERY active site x active material, with 0 where nothing was ever entered
CREATE OR REPLACE VIEW stock_overview AS
SELECT s.id AS site_id, s.name AS site, m.id AS material_id, m.name AS material, m.unit,
       COALESCE(cs.balance, 0) AS balance
FROM sites s
CROSS JOIN materials m
LEFT JOIN current_stock cs ON cs.site_id = s.id AND cs.material_id = m.id
WHERE s.is_active AND m.is_active;

CREATE OR REPLACE VIEW current_pallet_stock AS
SELECT site_id, pallet_type_id,
       SUM(pallet_sign(type) * quantity) AS balance
FROM pallet_txns GROUP BY site_id, pallet_type_id;

CREATE OR REPLACE VIEW pallet_overview AS
SELECT s.id AS site_id, s.name AS site,
       p.id AS pallet_type_id, p.name AS pallet_type,
       COALESCE(cps.balance, 0) AS balance
FROM sites s
CROSS JOIN pallet_types p
LEFT JOIN current_pallet_stock cps
       ON cps.site_id = s.id AND cps.pallet_type_id = p.id
WHERE s.is_active AND p.is_active;


---- Append Only
CREATE OR REPLACE FUNCTION block_truncate() RETURNS trigger AS $$
BEGIN
  RAISE EXCEPTION 'TRUNCATE is not allowed on %', TG_TABLE_NAME;
END;
$$ LANGUAGE plpgsql;

CREATE OR REPLACE FUNCTION audit_log_block_changes() RETURNS trigger AS $$
BEGIN
  RAISE EXCEPTION 'audit_log is append-only';
END;
$$ LANGUAGE plpgsql;

DROP TRIGGER IF EXISTS audit_log_no_update_delete ON audit_log;
CREATE TRIGGER audit_log_no_update_delete
BEFORE UPDATE OR DELETE ON audit_log
FOR EACH ROW EXECUTE FUNCTION audit_log_block_changes();

DROP TRIGGER IF EXISTS audit_log_no_truncate ON audit_log;
CREATE TRIGGER audit_log_no_truncate BEFORE TRUNCATE ON audit_log
FOR EACH STATEMENT EXECUTE FUNCTION block_truncate();

DROP TRIGGER IF EXISTS stock_txns_no_truncate ON stock_txns;
CREATE TRIGGER stock_txns_no_truncate BEFORE TRUNCATE ON stock_txns
FOR EACH STATEMENT EXECUTE FUNCTION block_truncate();

DROP TRIGGER IF EXISTS pallet_txns_no_truncate ON pallet_txns;
CREATE TRIGGER pallet_txns_no_truncate BEFORE TRUNCATE ON pallet_txns
FOR EACH STATEMENT EXECUTE FUNCTION block_truncate();

DROP TRIGGER IF EXISTS pallet_production_no_truncate ON pallet_production;
CREATE TRIGGER pallet_production_no_truncate BEFORE TRUNCATE ON pallet_production
FOR EACH STATEMENT EXECUTE FUNCTION block_truncate();

CREATE OR REPLACE FUNCTION stock_txns_force_now() RETURNS trigger AS $$
BEGIN
  NEW.txn_date   := (now() AT TIME ZONE 'Asia/Kolkata')::date;
  NEW.created_at := now();
  NEW.updated_at := NULL;
  RETURN NEW;
END;
$$ LANGUAGE plpgsql;

DROP TRIGGER IF EXISTS trg_0_force_now ON stock_txns;
CREATE TRIGGER trg_0_force_now
BEFORE INSERT ON stock_txns
FOR EACH ROW EXECUTE FUNCTION stock_txns_force_now();

CREATE OR REPLACE FUNCTION pallet_force_now() RETURNS trigger AS $$
BEGIN
  NEW.txn_date   := (now() AT TIME ZONE 'Asia/Kolkata')::date;
  NEW.created_at := now();
  IF TG_TABLE_NAME = 'pallet_production' THEN
    NEW.updated_at := NULL;
  END IF;
  RETURN NEW;
END;
$$ LANGUAGE plpgsql;

DROP TRIGGER IF EXISTS trg_pt_force_now ON pallet_txns;
CREATE TRIGGER trg_pt_force_now BEFORE INSERT ON pallet_txns
FOR EACH ROW EXECUTE FUNCTION pallet_force_now();

DROP TRIGGER IF EXISTS trg_pp_force_now ON pallet_production;
CREATE TRIGGER trg_pp_force_now BEFORE INSERT ON pallet_production
FOR EACH ROW EXECUTE FUNCTION pallet_force_now();

CREATE OR REPLACE FUNCTION edit_window() RETURNS interval AS $$
SELECT interval '1 hour' $$ LANGUAGE sql IMMUTABLE;


CREATE OR REPLACE FUNCTION stock_txns_guard() RETURNS trigger AS $$
DECLARE
  v_uid INT := NULLIF(current_setting('app.user_id', true), '')::int;
BEGIN
  IF v_uid IS NULL THEN
    RAISE EXCEPTION 'app.user_id is not set';
  END IF;

  IF v_uid <> OLD.created_by THEN
    RAISE EXCEPTION 'Only the creator can edit or delete this entry';
  END IF;

  IF OLD.transfer_ref IS NOT NULL THEN
    RAISE EXCEPTION 'Transfer entries cannot be edited or deleted';
  END IF;

  IF OLD.production_id IS NOT NULL THEN
    RAISE EXCEPTION 'Production entries cannot be edited or deleted';
  END IF;

  IF now() - OLD.created_at > edit_window() THEN
    RAISE EXCEPTION 'This entry is locked and can no longer be changed. Ask the admin to correct it.';
  END IF;

  IF TG_OP = 'UPDATE' THEN
    IF NEW.id            <> OLD.id
    OR NEW.site_id       <> OLD.site_id
    OR NEW.material_id   <> OLD.material_id
    OR NEW.type          <> OLD.type
    OR NEW.txn_date      <> OLD.txn_date
    OR NEW.created_by    <> OLD.created_by
    OR NEW.created_at    <> OLD.created_at
    OR NEW.transfer_ref  IS DISTINCT FROM OLD.transfer_ref
    OR NEW.production_id IS DISTINCT FROM OLD.production_id
    OR NEW.party         IS DISTINCT FROM OLD.party THEN
      RAISE EXCEPTION 'Only quantity and note can be edited';
    END IF;
    NEW.updated_at := now();
  END IF;

  INSERT INTO audit_log (table_name, record_id, action, old_data, new_data, changed_by)
  VALUES ('stock_txns', OLD.id, TG_OP, to_jsonb(OLD), CASE WHEN TG_OP = 'UPDATE' THEN to_jsonb(NEW) END, v_uid);

  IF TG_OP = 'DELETE' THEN RETURN OLD; END IF;
  RETURN NEW;
END;
$$ LANGUAGE plpgsql;

DROP TRIGGER IF EXISTS trg_1_guard ON stock_txns;
CREATE TRIGGER trg_1_guard
BEFORE UPDATE OR DELETE ON stock_txns
FOR EACH ROW EXECUTE FUNCTION stock_txns_guard();

CREATE OR REPLACE FUNCTION stock_txns_no_negative() RETURNS trigger AS $$
DECLARE
  v_site  INT;
  v_mat   INT;
  v_delta NUMERIC := 0;
  v_bal   NUMERIC;
BEGIN
  IF TG_OP = 'INSERT' THEN
    v_site := NEW.site_id; v_mat := NEW.material_id;
    v_delta := txn_sign(NEW.type) * NEW.quantity;
  ELSIF TG_OP = 'UPDATE' THEN
    v_site := NEW.site_id; v_mat := NEW.material_id;
    v_delta := txn_sign(NEW.type) * (NEW.quantity - OLD.quantity);
  ELSE  -- DELETE
    v_site := OLD.site_id; v_mat := OLD.material_id;
    v_delta := -txn_sign(OLD.type) * OLD.quantity;
  END IF;

  IF v_delta < 0 THEN
    PERFORM pg_advisory_xact_lock(v_site, v_mat);

    SELECT COALESCE(balance, 0) INTO v_bal
    FROM current_stock
    WHERE site_id = v_site AND material_id = v_mat;
    v_bal := COALESCE(v_bal, 0);

    IF v_bal + v_delta < 0 THEN
      RAISE EXCEPTION 'Not enough stock: balance is %, this change needs %', v_bal, -v_delta;
    END IF;
  END IF;

  IF TG_OP = 'DELETE' THEN RETURN OLD; END IF;
  RETURN NEW;
END;
$$ LANGUAGE plpgsql;

DROP TRIGGER IF EXISTS trg_2_no_negative ON stock_txns;
CREATE TRIGGER trg_2_no_negative
BEFORE INSERT OR UPDATE OR DELETE ON stock_txns
FOR EACH ROW EXECUTE FUNCTION stock_txns_no_negative();


CREATE OR REPLACE FUNCTION pallet_txns_no_negative() RETURNS trigger AS $$
DECLARE
  v_site INT; v_type INT; v_delta INT; v_bal INT;
BEGIN
  IF TG_OP = 'INSERT' THEN
    v_site := NEW.site_id; v_type := NEW.pallet_type_id;
    v_delta := pallet_sign(NEW.type) * NEW.quantity;
  ELSIF TG_OP = 'UPDATE' THEN
    v_site := NEW.site_id; v_type := NEW.pallet_type_id;
    v_delta := pallet_sign(NEW.type) * (NEW.quantity - OLD.quantity);
  ELSE
    v_site := OLD.site_id; v_type := OLD.pallet_type_id;
    v_delta := -pallet_sign(OLD.type) * OLD.quantity;
  END IF;

  IF v_delta < 0 THEN
    -- offset key so pallet locks don't collide with material locks
    PERFORM pg_advisory_xact_lock(v_site, v_type + 1000000);
    SELECT COALESCE(balance,0) INTO v_bal FROM current_pallet_stock
      WHERE site_id = v_site AND pallet_type_id = v_type;
    v_bal := COALESCE(v_bal,0);
    IF v_bal + v_delta < 0 THEN
      RAISE EXCEPTION 'Not enough pallets: balance is %, this change needs %',
        v_bal, -v_delta;
    END IF;
  END IF;

  IF TG_OP = 'DELETE' THEN RETURN OLD; END IF;
  RETURN NEW;
END;
$$ LANGUAGE plpgsql;

DROP TRIGGER IF EXISTS trg_pt_no_negative ON pallet_txns;
CREATE TRIGGER trg_pt_no_negative BEFORE INSERT OR UPDATE OR DELETE ON pallet_txns
FOR EACH ROW EXECUTE FUNCTION pallet_txns_no_negative();

-- Pallet rules: only the creator, only in the first hour, only bought/issued rows, audited
CREATE OR REPLACE FUNCTION pallet_txns_guard() RETURNS trigger AS $$
DECLARE
  v_uid INT := NULLIF(current_setting('app.user_id', true), '')::int;
BEGIN
  IF v_uid IS NULL THEN RAISE EXCEPTION 'app.user_id is not set'; END IF;
  IF v_uid <> OLD.created_by THEN
    RAISE EXCEPTION 'Only the creator can edit or delete this entry';
  END IF;
  IF OLD.production_id IS NOT NULL OR OLD.type NOT IN ('IN','OUT') THEN
    RAISE EXCEPTION 'Production and correction entries cannot be edited or deleted';
  END IF;
  IF now() - OLD.created_at > edit_window() THEN
    RAISE EXCEPTION 'This entry is locked and can no longer be changed. Ask the admin to correct it.';
  END IF;
  IF TG_OP = 'UPDATE' THEN
    IF NEW.id <> OLD.id OR NEW.site_id <> OLD.site_id OR NEW.pallet_type_id <> OLD.pallet_type_id
       OR NEW.type <> OLD.type OR NEW.txn_date <> OLD.txn_date OR NEW.created_by <> OLD.created_by
       OR NEW.created_at <> OLD.created_at
       OR NEW.production_id IS DISTINCT FROM OLD.production_id THEN
      RAISE EXCEPTION 'Only quantity, party and note can be edited';
    END IF;
  END IF;
  INSERT INTO audit_log (table_name, record_id, action, old_data, new_data, changed_by)
  VALUES ('pallet_txns', OLD.id, TG_OP, to_jsonb(OLD),
          CASE WHEN TG_OP = 'UPDATE' THEN to_jsonb(NEW) END, v_uid);
  IF TG_OP = 'DELETE' THEN RETURN OLD; END IF;
  RETURN NEW;
END;
$$ LANGUAGE plpgsql;

DROP TRIGGER IF EXISTS trg_pt_guard ON pallet_txns;
CREATE TRIGGER trg_pt_guard BEFORE UPDATE OR DELETE ON pallet_txns
FOR EACH ROW EXECUTE FUNCTION pallet_txns_guard();

-- Production records can never be edited or deleted (undo = admin reversal)
CREATE OR REPLACE FUNCTION pallet_production_immutable() RETURNS trigger AS $$
BEGIN
  RAISE EXCEPTION 'pallet_production rows cannot be edited; ask the admin to reverse the run';
END;
$$ LANGUAGE plpgsql;

DROP TRIGGER IF EXISTS trg_pp_immutable ON pallet_production;
CREATE TRIGGER trg_pp_immutable BEFORE UPDATE OR DELETE ON pallet_production
FOR EACH ROW EXECUTE FUNCTION pallet_production_immutable();

-- CFT factor history is append-only
CREATE OR REPLACE FUNCTION pallet_type_cft_immutable() RETURNS trigger AS $$
BEGIN
  RAISE EXCEPTION 'pallet_type_cft rows cannot be edited; set a new factor instead';
END;
$$ LANGUAGE plpgsql;

DROP TRIGGER IF EXISTS trg_ptc_immutable ON pallet_type_cft;
CREATE TRIGGER trg_ptc_immutable BEFORE UPDATE OR DELETE ON pallet_type_cft
FOR EACH ROW EXECUTE FUNCTION pallet_type_cft_immutable();

INSERT INTO sites (name, location) VALUES
  ('Caldreys Gujarat', 'Wankaner'),
  ('TRL Gujarat',      'Juna Katariya'),
  ('Caldreys Orrisa',  'Orrisa')
ON CONFLICT (name) DO NOTHING;





--- NEED CHANGE HERE!!!
INSERT INTO materials (name, unit) VALUES
  ('Cement','bags'), ('Sand','tons'), ('Steel','kg')
ON CONFLICT (name) DO NOTHING;

INSERT INTO materials (name, unit) VALUES ('Wood','CFT')
ON CONFLICT (name) DO NOTHING;

-- First admin: only created when you pass  -v admin_hash='...'
\if :{?admin_hash}
INSERT INTO users (name, username, password_hash, role)
VALUES ('Admin', 'admin', :'admin_hash', 'admin')
ON CONFLICT (username) DO NOTHING;
\endif

COMMIT;
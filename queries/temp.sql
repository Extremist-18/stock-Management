-- =====================================================================
-- schema.sql  -  the ONLY SQL file you need.
-- Replaces: init.sql, rules.sql, fixup.sql, approvals.sql, seed.sql
--
-- Safe to run any number of times. One transaction: all or nothing
--   First time (creates everything + the first admin):
--     ./build/inventory hashpw 'YourAdminPassword'      # prints a hash
--     psql "$DATABASE_URL" -v ON_ERROR_STOP=1 -v admin_hash='<hash>' -f queries/schema.sql
--
--   Later (just re-apply rules/schema changes):
--     psql "$DATABASE_URL" -v ON_ERROR_STOP=1 -f queries/schema.sql
-- =====================================================================
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

-- duplicate-tap guard: the app sends a fresh UUID per entry; the same UUID can only be saved once
ALTER TABLE stock_txns ADD COLUMN IF NOT EXISTS client_ref UUID;
CREATE UNIQUE INDEX IF NOT EXISTS stock_txns_client_ref_key ON stock_txns (client_ref);

ALTER TABLE stock_txns DROP CONSTRAINT IF EXISTS stock_txns_type_check;
ALTER TABLE stock_txns DROP CONSTRAINT IF EXISTS stock_txns_quantity_check;
ALTER TABLE stock_txns ADD CONSTRAINT stock_txns_quantity_check CHECK (quantity>0);

ALTER TABLE stock_txns ADD CONSTRAINT stock_txns_type_check CHECK (type IN
  ('IN','USAGE','ADMIN_PLUS','ADMIN_MINUS','TRANSFER_IN','TRANSFER_OUT',
   'BORROW_IN','BORROW_RETURN','LEND_OUT','LEND_RETURN','REJECT','REPROCESS_IN'));

CREATE INDEX IF NOT EXISTS stock_txns_lookup_idx ON stock_txns (site_id, material_id, txn_date);
-- per site+material: how long a delivery takes, and how many spare days you want on arrival
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
-- two people cannot both have a PENDING request for the same username
CREATE UNIQUE INDEX IF NOT EXISTS user_requests_one_pending
  ON user_requests (username) WHERE status = 'PENDING';




CREATE OR REPLACE FUNCTION txn_sign(t TEXT) RETURNS INT AS $$
  SELECT CASE WHEN t IN ('IN','ADMIN_PLUS','TRANSFER_IN','BORROW_IN','LEND_RETURN','REPROCESS_IN')
              THEN 1 ELSE -1 END;
$$ LANGUAGE sql IMMUTABLE;

-- only site+material pairs that already have at least one entry
CREATE OR REPLACE VIEW current_stock AS
SELECT site_id, material_id, SUM(txn_sign(type) * quantity) AS balance
FROM stock_txns GROUP BY site_id, material_id;

-- EVERY active site x active material, with 0 where nothing was ever entered.
CREATE OR REPLACE VIEW stock_overview AS
SELECT s.id AS site_id, s.name AS site, m.id AS material_id, m.name AS material, m.unit,
       COALESCE(cs.balance, 0) AS balance
FROM sites s
CROSS JOIN materials m
LEFT JOIN current_stock cs ON cs.site_id = s.id AND cs.material_id = m.id
WHERE s.is_active AND m.is_active;



-- Rule 1: audit_log is append-only (no UPDATE / DELETE / TRUNCATE)
CREATE OR REPLACE FUNCTION audit_log_block_changes() RETURNS trigger AS $$
BEGIN
  RAISE EXCEPTION 'audit_log is append-only';
END;
$$ LANGUAGE plpgsql;

DROP TRIGGER IF EXISTS audit_log_no_update_delete ON audit_log;
CREATE TRIGGER audit_log_no_update_delete
BEFORE UPDATE OR DELETE ON audit_log
FOR EACH ROW EXECUTE FUNCTION audit_log_block_changes();

CREATE OR REPLACE FUNCTION block_truncate() RETURNS trigger AS $$
BEGIN
  RAISE EXCEPTION 'TRUNCATE is not allowed on %', TG_TABLE_NAME;
END;
$$ LANGUAGE plpgsql;

DROP TRIGGER IF EXISTS audit_log_no_truncate ON audit_log;
CREATE TRIGGER audit_log_no_truncate BEFORE TRUNCATE ON audit_log
FOR EACH STATEMENT EXECUTE FUNCTION block_truncate();

DROP TRIGGER IF EXISTS stock_txns_no_truncate ON stock_txns;
CREATE TRIGGER stock_txns_no_truncate BEFORE TRUNCATE ON stock_txns
FOR EACH STATEMENT EXECUTE FUNCTION block_truncate();


-- Rule 2: NO BACK-DATING. Whatever a new row says, the database stamps
-- today's date (India time) and the current moment.
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

CREATE OR REPLACE FUNCTION edit_window() RETURNS interval AS $$
SELECT interval '1 hour' $$ LANGUAGE sql IMMUTABLE;

-- Rule 3: only the creator, only in the first hour, only quantity + note;
-- transfer entries can never be edited/deleted; every change audited.
CREATE OR REPLACE FUNCTION stock_txns_guard() RETURNS trigger AS $$
DECLARE
  v_uid INT := NULLIF(current_setting('app.user_id', true), '')::int;
BEGIN
  IF v_uid IS NULL THEN
    RAISE EXCEPTION 'app.user_id is not set';
  END IF;

  -- only the person who made the entry can change it
  IF v_uid <> OLD.created_by THEN
    RAISE EXCEPTION 'Only the creator can edit or delete this entry';
  END IF;

  -- the two halves of a transfer must always stay matched
  IF OLD.transfer_ref IS NOT NULL THEN
    RAISE EXCEPTION 'Transfer entries cannot be edited or deleted';
  END IF;

  -- 1-hour window, measured by the database clock
  IF now() - OLD.created_at > edit_window() THEN
    RAISE EXCEPTION 'This entry is locked and can no longer be changed. Ask the admin to correct it.';
  END IF;
  
  IF TG_OP = 'UPDATE' THEN
    IF NEW.id           <> OLD.id
    OR NEW.site_id      <> OLD.site_id
    OR NEW.material_id  <> OLD.material_id
    OR NEW.type         <> OLD.type
    OR NEW.txn_date     <> OLD.txn_date
    OR NEW.created_by   <> OLD.created_by
    OR NEW.created_at   <> OLD.created_at
    OR NEW.transfer_ref IS DISTINCT FROM OLD.transfer_ref
    OR NEW.party        IS DISTINCT FROM OLD.party THEN
      RAISE EXCEPTION 'Only quantity and note can be edited';
    END IF;
    NEW.updated_at := now();
  END IF;

  INSERT INTO audit_log (table_name, record_id, action, old_data, new_data, changed_by)
  VALUES ('stock_txns', OLD.id, TG_OP, to_jsonb(OLD),
          CASE WHEN TG_OP = 'UPDATE' THEN to_jsonb(NEW) END, v_uid);

  IF TG_OP = 'DELETE' THEN RETURN OLD; END IF;
  RETURN NEW;
END;
$$ LANGUAGE plpgsql;

DROP TRIGGER IF EXISTS trg_1_guard ON stock_txns;
CREATE TRIGGER trg_1_guard
BEFORE UPDATE OR DELETE ON stock_txns
FOR EACH ROW EXECUTE FUNCTION stock_txns_guard();

-- Rule 4: stock can never go below zero (inserts, edits and deletes)
CREATE OR REPLACE FUNCTION stock_txns_no_negative() RETURNS trigger AS $$
DECLARE
  v_site  INT;
  v_mat   INT;
  v_delta NUMERIC := 0;      -- change this operation makes to the balance
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
    -- one at a time per site+material, so two simultaneous entries can't both pass
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


INSERT INTO sites (name, location) VALUES
  ('Caldreys Gujarat', 'Wankaner'),
  ('TRL Gujarat',      'Juna Katariya'),
  ('Caldreys Orrisa',  'Orrisa')
ON CONFLICT (name) DO NOTHING;

INSERT INTO materials (name, unit) VALUES
  ('Cement','bags'), ('Sand','tons'), ('Steel','kg')
ON CONFLICT (name) DO NOTHING;

-- First admin: only created when you pass  -v admin_hash='...'
-- (other users are created later from the app by the admin)
\if :{?admin_hash}
INSERT INTO users (name, username, password_hash, role)
VALUES ('Admin', 'admin', :'admin_hash', 'admin')
ON CONFLICT (username) DO NOTHING;
\endif

COMMIT;
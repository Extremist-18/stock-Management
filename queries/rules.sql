-- ============ Helper: does this type add (+1) or subtract (-1)? ============
CREATE OR REPLACE FUNCTION txn_sign(t TEXT) RETURNS INT AS $$
  SELECT CASE WHEN t IN ('IN','ADMIN_PLUS','TRANSFER_IN','BORROW_IN','LEND_RETURN','REPROCESS_IN') THEN 1 ELSE -1 END;
$$ LANGUAGE sql IMMUTABLE;


-- ============ Rule 1: audit_log is append-only ============
CREATE OR REPLACE FUNCTION audit_log_block_changes() RETURNS trigger AS $$
BEGIN
  RAISE EXCEPTION 'audit_log is append-only';
END;
$$ LANGUAGE plpgsql;

DROP TRIGGER IF EXISTS audit_log_no_update_delete ON audit_log;
CREATE TRIGGER audit_log_no_update_delete
BEFORE UPDATE OR DELETE ON audit_log
FOR EACH ROW EXECUTE FUNCTION audit_log_block_changes();


-- ============ Rules 2 + 3: creator-only, 1-hour lock, immutable fields, audit ============
CREATE OR REPLACE FUNCTION stock_txns_guard() RETURNS trigger AS $$
DECLARE
  v_uid INT := NULLIF(current_setting('app.user_id', true), '')::int;
BEGIN
  -- the C++ server must always say who is acting
  IF v_uid IS NULL THEN
    RAISE EXCEPTION 'app.user_id is not set';
  END IF;

  -- only the person who made the entry can change it
  IF v_uid <> OLD.created_by THEN
    RAISE EXCEPTION 'Only the creator can edit or delete this entry';
  END IF;

  -- 1-hour window, measured by the database clock
  IF now() - OLD.created_at > interval '1 hour' THEN
    RAISE EXCEPTION 'Entry is locked: the 1-hour edit window has passed';
  END IF;

  IF TG_OP = 'UPDATE' THEN
    IF NEW.site_id      <> OLD.site_id
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
  VALUES ('stock_txns', OLD.id, TG_OP, to_jsonb(OLD), CASE WHEN TG_OP = 'UPDATE' THEN to_jsonb(NEW) END, v_uid);

  IF TG_OP = 'DELETE' THEN RETURN OLD; END IF;
  RETURN NEW;
END;
$$ LANGUAGE plpgsql;

DROP TRIGGER IF EXISTS trg_1_guard ON stock_txns;
CREATE TRIGGER trg_1_guard
BEFORE UPDATE OR DELETE ON stock_txns
FOR EACH ROW EXECUTE FUNCTION stock_txns_guard();


-- ============ Rule 4: stock can never go below zero ============
-- Covers inserts, edits and deletes (deleting an IN can also cause a negative).
CREATE OR REPLACE FUNCTION stock_txns_no_negative() RETURNS trigger AS $$
DECLARE
  v_site INT;
  v_mat  INT;
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

  -- only operations that reduce stock need checking
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
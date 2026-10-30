-- 1. columns/types the later design needs
ALTER TABLE stock_txns ADD COLUMN IF NOT EXISTS party TEXT;
ALTER TABLE sites ADD COLUMN IF NOT EXISTS is_active BOOLEAN NOT NULL DEFAULT TRUE;
CREATE INDEX IF NOT EXISTS stock_txns_lookup_idx ON stock_txns (site_id, material_id, txn_date);

-- 2. one agreed list of transaction types
ALTER TABLE stock_txns DROP CONSTRAINT IF EXISTS stock_txns_type_check;
ALTER TABLE stock_txns ADD CONSTRAINT stock_txns_type_check CHECK (type IN
  ('IN','USAGE','ADMIN_PLUS','ADMIN_MINUS','TRANSFER_IN','TRANSFER_OUT',
   'BORROW_IN','BORROW_RETURN','LEND_OUT','LEND_RETURN','REJECT','REPROCESS_IN'));

-- 3. the balance view now uses txn_sign() from rules.sql: ONE place decides add vs subtract
CREATE OR REPLACE VIEW current_stock AS
SELECT site_id, material_id, SUM(txn_sign(type) * quantity) AS balance
FROM stock_txns GROUP BY site_id, material_id;
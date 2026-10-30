 CREATE TABLE sites (
  id       SERIAL PRIMARY KEY,
  name     TEXT NOT NULL,
  location TEXT
);

INSERT INTO sites (name, location) VALUES
  ('Caldreys Gujarat', 'Wankaner'),
  ('TRL Gujarat', 'Juna Katariya'),
  ('Caldreys Orrisa', 'Orrisa');

CREATE TABLE users (
  id            SERIAL PRIMARY KEY,
  name          TEXT NOT NULL,
  username      TEXT NOT NULL UNIQUE,
  password_hash TEXT NOT NULL,
  role          TEXT NOT NULL CHECK (role IN ('admin','supervisor')),
  is_active     BOOLEAN NOT NULL DEFAULT TRUE,
  created_at    TIMESTAMPTZ NOT NULL DEFAULT now()
);

CREATE TABLE user_sites (
  user_id INT NOT NULL REFERENCES users(id),
  site_id INT NOT NULL REFERENCES sites(id),
  PRIMARY KEY (user_id, site_id)
);

CREATE TABLE materials (
  id        SERIAL PRIMARY KEY,
  name      TEXT NOT NULL UNIQUE,
  unit      TEXT NOT NULL,
  is_active BOOLEAN NOT NULL DEFAULT TRUE
);

CREATE TABLE stock_txns (
  id          BIGSERIAL PRIMARY KEY,
  site_id     INT NOT NULL REFERENCES sites(id),
  material_id INT NOT NULL REFERENCES materials(id),
  type        TEXT NOT NULL CHECK (type IN ('IN','USAGE','ADMIN_PLUS','ADMIN_MINUS','TRANSFER_IN','TRANSFER_OUT')),
  quantity    NUMERIC(14,3) NOT NULL CHECK (quantity > 0),
  txn_date    DATE NOT NULL DEFAULT ((now() AT TIME ZONE 'Asia/Kolkata')::date),
  note        TEXT,  
  transfer_ref UUID,          
  created_by  INT NOT NULL REFERENCES users(id),
  created_at  TIMESTAMPTZ NOT NULL DEFAULT now(),
  updated_at  TIMESTAMPTZ
);

CREATE TABLE audit_log (
  id         BIGSERIAL PRIMARY KEY,
  table_name TEXT NOT NULL,
  record_id  BIGINT,
  action     TEXT NOT NULL,       -- UPDATE, DELETE, LOGIN, LOGIN_FAILED, USER_CREATED...
  old_data   JSONB,
  new_data   JSONB,
  changed_by INT REFERENCES users(id),
  changed_at TIMESTAMPTZ NOT NULL DEFAULT now()
);

CREATE VIEW current_stock AS
SELECT site_id, material_id, SUM(CASE WHEN type IN ('IN','ADMIN_PLUS','TRANSFER_IN') THEN quantity ELSE -quantity END) AS balance
FROM stock_txns GROUP BY site_id, material_id;
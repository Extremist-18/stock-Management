CREATE TABLE IF NOT EXISTS transfer_requests (
  id            BIGSERIAL PRIMARY KEY,
  from_site_id  INT NOT NULL REFERENCES sites(id),
  to_site_id    INT NOT NULL REFERENCES sites(id),
  material_id   INT NOT NULL REFERENCES materials(id),
  quantity      NUMERIC(14,3) NOT NULL CHECK (quantity > 0),
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

CREATE INDEX IF NOT EXISTS transfer_requests_status_idx ON transfer_requests (status);
CREATE INDEX IF NOT EXISTS user_requests_status_idx ON user_requests (status);
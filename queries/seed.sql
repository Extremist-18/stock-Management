INSERT INTO users (name, username, password_hash, role) VALUES
  ('Admin',  'admin',  'dummy', 'admin'),
  ('Ramesh', 'ramesh', 'dummy', 'supervisor')
ON CONFLICT (username) DO NOTHING;

INSERT INTO materials (name, unit) VALUES
  ('Cement','bags'), ('Sand','tons'), ('Steel','kg')
ON CONFLICT (name) DO NOTHING;

INSERT INTO user_sites (user_id, site_id)
SELECT u.id, s.id FROM users u, sites s
WHERE u.username = 'ramesh' AND s.name = 'Caldreys Gujarat'
ON CONFLICT DO NOTHING;

INSERT INTO stock_txns (site_id, material_id, type, quantity, created_by, note)
SELECT s.id, m.id, 'IN', 200, u.id, 'test truck'
FROM sites s, materials m, users u
WHERE s.name = 'Caldreys Gujarat' AND m.name = 'Cement' AND u.username = 'ramesh';

INSERT INTO stock_txns (site_id, material_id, type, quantity, created_by, note)
SELECT s.id, m.id, 'USAGE', 40, u.id, 'test usage'
FROM sites s, materials m, users u
WHERE s.name = 'Caldreys Gujarat' AND m.name = 'Cement' AND u.username = 'ramesh';
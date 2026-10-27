SELECT info
FROM info_type
WHERE info ILIKE '%height%'
  OR info ILIKE '%weight%'
  OR info ILIKE '%physical%';

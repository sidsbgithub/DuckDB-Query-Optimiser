SELECT id,
       title,
       kind_id,
       production_year
FROM title
WHERE title = 'The Matrix'
  AND production_year = 1999;

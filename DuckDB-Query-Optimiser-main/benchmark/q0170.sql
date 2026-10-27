SELECT id,
       title,
       kind_id,
       production_year
FROM title
WHERE title ILIKE 'The Matrix%';

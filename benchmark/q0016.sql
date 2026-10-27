SELECT t.title,
       t.production_year
FROM cast_info ci
JOIN title t ON t.id = ci.movie_id
WHERE ci.person_id = 1148661
  AND ci.role_id = 8
LIMIT 10;

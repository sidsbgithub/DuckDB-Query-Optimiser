SELECT ci.movie_id,
       t.title,
       t.production_year
FROM cast_info ci
JOIN title t ON ci.movie_id = t.id
WHERE ci.person_id = 1697404
  AND ci.role_id = 8
ORDER BY t.production_year;

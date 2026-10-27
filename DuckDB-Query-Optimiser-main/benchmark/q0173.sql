SELECT n.name,
       rt.role
FROM cast_info ci
JOIN name n ON n.id = ci.person_id
JOIN role_type rt ON rt.id = ci.role_id
WHERE ci.movie_id = 2392833
  AND rt.role IN ('actor',
                  'actress')
ORDER BY ci.nr_order
LIMIT 15;

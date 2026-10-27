SELECT n.name,
       rt.role,
       ci.note
FROM cast_info ci
JOIN name n ON n.id = ci.person_id
JOIN role_type rt ON rt.id = ci.role_id
WHERE ci.movie_id = 2392833
ORDER BY ci.nr_order NULLS LAST
LIMIT 30;

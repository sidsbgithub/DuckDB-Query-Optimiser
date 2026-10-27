SELECT k.kind,
       COUNT(*)
FROM cast_info ci
JOIN title t ON t.id = ci.movie_id
JOIN kind_type k ON k.id = t.kind_id
WHERE ci.person_id = 1148661
  AND ci.role_id = 8
GROUP BY k.kind;

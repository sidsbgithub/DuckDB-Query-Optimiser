SELECT * FROM (
SELECT t.production_year AS year, rt.role, coalesce(n.gender, 'unknown') AS gender,
       count(*) AS n_credits, count(DISTINCT ci.person_id) AS n_people
FROM cast_info ci
JOIN title t ON t.id = ci.movie_id
JOIN kind_type kt ON kt.id = t.kind_id
JOIN name n ON n.id = ci.person_id
JOIN role_type rt ON rt.id = ci.role_id
WHERE t.production_year IS NOT NULL
  AND kt.kind IN ('episode', 'movie', 'video movie', 'tv movie', 'tv series', 'video game')
  AND EXISTS (SELECT 1 FROM movie_companies mcf JOIN company_name cnf ON cnf.id = mcf.company_id WHERE mcf.movie_id = t.id AND cnf.country_code = '[ca]')
GROUP BY t.production_year, rt.role, n.gender
ORDER BY t.production_year, rt.role, gender
) d
WHERE d.role = 'producer'
ORDER BY d.year, d.gender;

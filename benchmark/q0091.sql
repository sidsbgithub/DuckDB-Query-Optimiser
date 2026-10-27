SELECT cn.name AS company, count(*) AS n_titles
FROM title t
JOIN kind_type kt ON kt.id = t.kind_id
JOIN movie_companies mc ON mc.movie_id = t.id
JOIN company_name cn ON cn.id = mc.company_id
WHERE t.production_year BETWEEN 2006 AND 2006
  AND kt.kind IN ('episode', 'movie')
GROUP BY cn.name
ORDER BY n_titles DESC, company
LIMIT 20;

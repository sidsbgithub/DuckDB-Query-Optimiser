SELECT t.production_year AS year, count(*) AS n_titles
FROM title t
JOIN kind_type kt ON kt.id = t.kind_id
WHERE t.production_year BETWEEN 2001 AND 2011
  AND kt.kind IN ('episode', 'movie')
GROUP BY t.production_year
ORDER BY t.production_year;

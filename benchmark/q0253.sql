SELECT t.production_year AS year, avg(CAST(mii.info AS DOUBLE)) AS avg_rating, count(*) AS n_rated
FROM title t
JOIN kind_type kt ON kt.id = t.kind_id
JOIN movie_info_idx mii ON mii.movie_id = t.id
JOIN info_type it ON it.id = mii.info_type_id
WHERE t.production_year BETWEEN 1998 AND 2003
  AND kt.kind IN ('episode', 'movie', 'video movie', 'tv movie', 'tv series', 'video game')
  AND EXISTS (SELECT 1 FROM movie_companies mcf JOIN company_name cnf ON cnf.id = mcf.company_id WHERE mcf.movie_id = t.id AND cnf.country_code = '[ca]')
  AND it.info = 'rating'
GROUP BY t.production_year
ORDER BY t.production_year;

SELECT * FROM (
SELECT t.production_year AS year, mi.info AS genre, count(*) AS n_titles
FROM title t
JOIN kind_type kt ON kt.id = t.kind_id
JOIN movie_info mi ON mi.movie_id = t.id
JOIN info_type it ON it.id = mi.info_type_id
WHERE kt.kind IN ('episode', 'movie', 'video movie', 'tv movie', 'tv series', 'video game')
  AND EXISTS (SELECT 1 FROM movie_companies mcf JOIN company_name cnf ON cnf.id = mcf.company_id WHERE mcf.movie_id = t.id AND cnf.country_code = '[ca]')
  AND EXISTS (SELECT 1 FROM movie_info mif JOIN info_type itf ON itf.id = mif.info_type_id WHERE mif.movie_id = t.id AND itf.info = 'genres' AND mif.info = 'Drama')
  AND t.production_year IS NOT NULL
  AND it.info = 'genres'
GROUP BY t.production_year, mi.info
ORDER BY t.production_year, mi.info
) d
WHERE d.genre = 'Drama'
ORDER BY d.year;

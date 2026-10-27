SELECT mi.info AS genre, count(*) AS n_titles
FROM title t
JOIN kind_type kt ON kt.id = t.kind_id
JOIN movie_info mi ON mi.movie_id = t.id
JOIN info_type it ON it.id = mi.info_type_id
WHERE t.production_year BETWEEN 2001 AND 2011
  AND kt.kind IN ('episode', 'movie')
  AND it.info = 'genres'
GROUP BY mi.info
ORDER BY n_titles DESC, genre
LIMIT 15;

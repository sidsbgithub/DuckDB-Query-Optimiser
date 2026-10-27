SELECT t.title, t.production_year, kt.kind, t.imdb_index
FROM title t
JOIN kind_type kt ON kt.id = t.kind_id
LEFT JOIN (SELECT mii.movie_id, min(CAST(mii.info AS DOUBLE)) AS rating
           FROM movie_info_idx mii JOIN info_type itr ON itr.id = mii.info_type_id
           WHERE itr.info = 'rating' GROUP BY mii.movie_id) opt_r
    ON opt_r.movie_id = t.id
LEFT JOIN (SELECT mip.movie_id, min(mip.info) AS plot
           FROM movie_info mip JOIN info_type itp ON itp.id = mip.info_type_id
           WHERE itp.info = 'plot' GROUP BY mip.movie_id) opt_p
    ON opt_p.movie_id = t.id
WHERE t.production_year BETWEEN 2001 AND 2011
  AND kt.kind IN ('episode', 'movie')
ORDER BY t.production_year ASC, t.id
LIMIT 10000 OFFSET 0;

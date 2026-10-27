WITH t AS
  (SELECT id
   FROM title
   WHERE production_year BETWEEN 1995 AND 2005),
     r AS
  (SELECT movie_id,
          avg(CAST(info AS DOUBLE)) AS rating
   FROM movie_info_idx
   WHERE info_type_id = 101
   GROUP BY movie_id),
     b AS
  (SELECT DISTINCT movie_id
   FROM movie_info
   WHERE info_type_id = 105)
SELECT CASE
           WHEN b.movie_id IS NOT NULL THEN 'has_budget'
           ELSE 'no_budget'
       END AS grp,
       count(*) AS n_movies,
       avg(r.rating) AS avg_rating
FROM t
JOIN r ON r.movie_id = t.id
LEFT JOIN b ON b.movie_id = t.id
GROUP BY grp;

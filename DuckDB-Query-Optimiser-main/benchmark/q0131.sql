SELECT f.n_films,
       count(*)
FROM
  (SELECT person_id,
          count(DISTINCT movie_id) AS n_films
   FROM cast_info
   WHERE role_id IN (1,
                     2)
   GROUP BY person_id) f
WHERE f.n_films > 500
GROUP BY f.n_films;

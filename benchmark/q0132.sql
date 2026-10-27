WITH films AS
  (SELECT person_id,
          count(DISTINCT movie_id) AS n_films
   FROM cast_info
   WHERE role_id IN (1,
                     2)
   GROUP BY person_id)
SELECT min(n_films),
       max(n_films),
       avg(n_films),
       median(n_films)
FROM films;

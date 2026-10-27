WITH mc AS
  (SELECT DISTINCT ci.movie_id,
                   ci.person_id
   FROM cast_info ci
   JOIN title t ON ci.movie_id = t.id
   WHERE t.kind_id = 1
     AND t.production_year BETWEEN 1990 AND 2010
     AND ci.role_id IN (1,
                        2)),
     pairs AS
  (SELECT a.movie_id,
          a.person_id AS p1,
          b.person_id AS p2
   FROM mc a
   JOIN mc b ON a.movie_id = b.movie_id
   AND a.person_id < b.person_id)
SELECT p1,
       p2,
       count(*) AS num_movies
FROM pairs
GROUP BY p1,
         p2
ORDER BY num_movies DESC
LIMIT 20;

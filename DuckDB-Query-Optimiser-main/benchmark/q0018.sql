WITH directors AS
  (SELECT ci.movie_id,
          ci.person_id AS director_id
   FROM cast_info ci
   JOIN title t ON t.id = ci.movie_id
   WHERE ci.role_id = 8
     AND t.kind_id =
       (SELECT id
        FROM kind_type
        WHERE kind = 'movie')),
     cinematographers AS
  (SELECT ci.movie_id,
          ci.person_id AS cine_id
   FROM cast_info ci
   JOIN title t ON t.id = ci.movie_id
   WHERE ci.role_id = 5
     AND t.kind_id =
       (SELECT id
        FROM kind_type
        WHERE kind = 'movie'))
SELECT d.director_id,
       c.cine_id,
       COUNT(DISTINCT d.movie_id) AS films_together
FROM directors d
JOIN cinematographers c ON d.movie_id = c.movie_id
GROUP BY d.director_id,
         c.cine_id
ORDER BY films_together DESC
LIMIT 15;

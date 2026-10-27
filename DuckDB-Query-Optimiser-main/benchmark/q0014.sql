WITH directors AS
  (SELECT movie_id,
          person_id AS director_id
   FROM cast_info
   WHERE role_id = 8),
     cinematographers AS
  (SELECT movie_id,
          person_id AS cine_id
   FROM cast_info
   WHERE role_id = 5)
SELECT d.director_id,
       c.cine_id,
       COUNT(DISTINCT d.movie_id) AS films_together
FROM directors d
JOIN cinematographers c ON d.movie_id = c.movie_id
GROUP BY d.director_id,
         c.cine_id
ORDER BY films_together DESC
LIMIT 15;

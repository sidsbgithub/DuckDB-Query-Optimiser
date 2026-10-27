WITH wkw_movies AS
  (SELECT ci.movie_id
   FROM cast_info ci
   WHERE ci.person_id = 1697404
     AND ci.role_id = 8)
SELECT mi.info AS genre,
       count(*) c
FROM movie_info mi
JOIN wkw_movies wm ON mi.movie_id = wm.movie_id
WHERE mi.info_type_id = 3
GROUP BY mi.info
ORDER BY c DESC;

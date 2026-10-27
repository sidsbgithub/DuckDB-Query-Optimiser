WITH wkw_movies AS
  (SELECT ci.movie_id
   FROM cast_info ci
   WHERE ci.person_id = 1697404
     AND ci.role_id = 8)
SELECT k.keyword,
       count(*) c
FROM movie_keyword mk
JOIN wkw_movies wm ON mk.movie_id = wm.movie_id
JOIN keyword k ON k.id = mk.keyword_id
GROUP BY k.keyword
ORDER BY c DESC
LIMIT 30;

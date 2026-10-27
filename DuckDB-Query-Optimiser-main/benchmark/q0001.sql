SELECT k.keyword,
       count(*) c
FROM movie_keyword mk
JOIN keyword k ON k.id=mk.keyword_id
JOIN title t ON t.id=mk.movie_id
WHERE t.production_year BETWEEN 2000 AND 2009
GROUP BY k.keyword
ORDER BY c DESC
LIMIT 15;

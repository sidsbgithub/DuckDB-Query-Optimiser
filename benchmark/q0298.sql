SELECT k.keyword,
       count(*) AS cnt
FROM movie_companies mc
JOIN company_type ct ON ct.id = mc.company_type_id
JOIN movie_keyword mk ON mk.movie_id = mc.movie_id
JOIN keyword k ON k.id = mk.keyword_id
WHERE mc.company_id = 1310
  AND ct.kind = 'production companies'
GROUP BY k.keyword
ORDER BY cnt DESC
LIMIT 20;

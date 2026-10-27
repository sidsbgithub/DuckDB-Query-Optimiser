SELECT k.keyword, count(*) AS n_titles
FROM title t
JOIN kind_type kt ON kt.id = t.kind_id
JOIN movie_keyword mk ON mk.movie_id = t.id
JOIN keyword k ON k.id = mk.keyword_id
WHERE t.production_year BETWEEN 1998 AND 2003
  AND kt.kind IN ('episode', 'movie', 'video movie', 'tv movie', 'tv series', 'video game')
  AND EXISTS (SELECT 1 FROM movie_companies mcf JOIN company_name cnf ON cnf.id = mcf.company_id WHERE mcf.movie_id = t.id AND cnf.country_code = '[ca]')
GROUP BY k.keyword
ORDER BY n_titles DESC, k.keyword
LIMIT 25;

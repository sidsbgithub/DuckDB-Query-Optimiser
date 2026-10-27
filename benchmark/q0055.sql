WITH movies_90s AS
  (SELECT id
   FROM title
   WHERE production_year BETWEEN 1990 AND 1999),
     us_movies AS
  (SELECT DISTINCT mc.movie_id
   FROM movie_companies mc
   JOIN company_name cn ON mc.company_id = cn.id
   JOIN movies_90s m ON m.id = mc.movie_id
   WHERE cn.country_code = '[us]'),
     nonus_movies AS
  (SELECT DISTINCT mc.movie_id
   FROM movie_companies mc
   JOIN company_name cn ON mc.company_id = cn.id
   JOIN movies_90s m ON m.id = mc.movie_id
   WHERE cn.country_code IS NOT NULL
     AND cn.country_code <> '[us]'),
     classified AS
  (SELECT id AS movie_id,
          CASE
              WHEN id IN
                     (SELECT movie_id
                      FROM us_movies)
                   AND id IN
                     (SELECT movie_id
                      FROM nonus_movies) THEN 'mixed'
              WHEN id IN
                     (SELECT movie_id
                      FROM us_movies) THEN 'us_only'
              WHEN id IN
                     (SELECT movie_id
                      FROM nonus_movies) THEN 'nonus_only'
              ELSE 'unknown'
          END AS grp
   FROM movies_90s)
SELECT c.grp,
       n.gender,
       count(*) AS credits
FROM classified c
JOIN cast_info ci ON ci.movie_id = c.movie_id
JOIN name n ON n.id = ci.person_id
WHERE c.grp = 'mixed'
  AND n.gender IN ('f',
                   'm')
GROUP BY c.grp,
         n.gender;

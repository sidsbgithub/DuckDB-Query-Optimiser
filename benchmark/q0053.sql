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
     AND cn.country_code <> '[us]')
SELECT
  (SELECT count(*)
   FROM us_movies) AS us_only_or_mixed,

  (SELECT count(*)
   FROM nonus_movies) AS nonus_only_or_mixed,

  (SELECT count(*)
   FROM us_movies u
   WHERE u.movie_id NOT IN
       (SELECT movie_id
        FROM nonus_movies)) AS us_exclusive,

  (SELECT count(*)
   FROM nonus_movies n
   WHERE n.movie_id NOT IN
       (SELECT movie_id
        FROM us_movies)) AS nonus_exclusive,

  (SELECT count(*)
   FROM us_movies u
   WHERE u.movie_id IN
       (SELECT movie_id
        FROM nonus_movies)) AS mixed;

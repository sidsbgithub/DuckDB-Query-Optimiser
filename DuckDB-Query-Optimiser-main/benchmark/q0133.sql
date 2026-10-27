WITH RAW AS
  (SELECT pi.person_id,
          pi.info
   FROM person_info pi
   JOIN info_type it ON pi.info_type_id = it.id
   WHERE it.info = 'height'), parsed AS
  (SELECT person_id,
          CASE
              WHEN info ILIKE '%cm%' THEN TRY_CAST(regexp_extract(info, '([0-9]+(\.[0-9]+)?)', 1) AS DOUBLE)
              WHEN info SIMILAR TO '[0-9]+''.*' THEN TRY_CAST(regexp_extract(info, '^([0-9]+)' || chr(39), 1) AS DOUBLE) * 30.48 + COALESCE(TRY_CAST(regexp_extract(info, chr(39) || '\s*([0-9]+)', 1) AS DOUBLE), 0) * 2.54
              ELSE NULL
          END AS height_cm
   FROM RAW),
                              clean AS
  (SELECT person_id,
          AVG(height_cm) AS height_cm
   FROM parsed
   WHERE height_cm BETWEEN 130 AND 230
   GROUP BY person_id),
                              films_movie_only AS
  (SELECT ci.person_id,
          count(DISTINCT ci.movie_id) AS n_films
   FROM cast_info ci
   JOIN title t ON ci.movie_id = t.id
   WHERE ci.role_id IN (1,
                        2)
     AND t.kind_id = 1
   GROUP BY ci.person_id)
SELECT count(*) AS n_actors,
       corr(c.height_cm, f.n_films) AS pearson_corr_movies,
       avg(f.n_films) AS avg_films,
       median(f.n_films) AS median_films,
       max(f.n_films) AS max_films
FROM clean c
JOIN films_movie_only f ON c.person_id = f.person_id;

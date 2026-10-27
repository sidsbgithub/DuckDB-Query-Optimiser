SELECT max(cnt)
FROM
  (SELECT t.id,
          count(*) cnt
   FROM cast_info ci
   JOIN title t ON ci.movie_id=t.id
   WHERE t.kind_id=1
     AND t.production_year BETWEEN 1990 AND 2010
     AND ci.role_id IN (1,
                        2)
   GROUP BY t.id);

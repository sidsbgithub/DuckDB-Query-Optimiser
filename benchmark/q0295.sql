SELECT mc.company_id,
       cn.name,
       ct.kind,
       count(*)
FROM movie_companies mc
JOIN company_name cn ON cn.id=mc.company_id
JOIN company_type ct ON ct.id=mc.company_type_id
WHERE cn.id IN (18166,
                1310)
GROUP BY 1,
         2,
         3
ORDER BY 4 DESC;

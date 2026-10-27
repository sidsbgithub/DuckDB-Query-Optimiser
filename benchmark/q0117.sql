SELECT count(*) FILTER (
                        WHERE e.production_year IS NOT NULL
                          AND s.production_year IS NOT NULL) AS both_known,
       count(*) FILTER (
                        WHERE e.production_year = s.production_year) AS same_year
FROM title e
JOIN title s ON e.episode_of_id = s.id
WHERE e.kind_id = 7;

SELECT count(*) AS total_episodes,
       count(s.production_year) AS series_has_year,
       count(e.production_year) AS ep_has_year,
       count(*) FILTER (
                        WHERE e.production_year = s.production_year) AS same_year
FROM title e
JOIN title s ON e.episode_of_id = s.id
WHERE e.kind_id = 7;

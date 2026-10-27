SELECT pi.info,
       count(*)
FROM person_info pi
JOIN info_type it ON pi.info_type_id = it.id
WHERE it.info='height'
  AND pi.info NOT SIMILAR TO '[0-9]''( [0-9]+( [0-9]+/[0-9]+)?"?)?'
  AND pi.info NOT SIMILAR TO '[0-9]+(\.[0-9]+)? cm'
GROUP BY pi.info
ORDER BY 2 DESC
LIMIT 40;

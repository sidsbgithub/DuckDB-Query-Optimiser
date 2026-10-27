SELECT pi.info,
       count(*)
FROM person_info pi
JOIN info_type it ON pi.info_type_id = it.id
WHERE it.info = 'height'
GROUP BY pi.info
ORDER BY count(*) DESC
LIMIT 30;

SELECT count(*)
FROM person_info pi
JOIN info_type it ON pi.info_type_id = it.id
WHERE it.info = 'height';

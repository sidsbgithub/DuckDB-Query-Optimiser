WITH RAW AS
  (SELECT pi.person_id,
          pi.info
   FROM person_info pi
   JOIN info_type it ON pi.info_type_id = it.id
   WHERE it.info = 'height'), parsed AS
  (SELECT person_id,
          info,
          CASE WHEN info ILIKE '%cm%' THEN TRY_CAST(regexp_extract(info, '([0-9]+(\.[0-9]+)?)', 1) AS DOUBLE) WHEN info SIMILAR TO '[0-9]+''.*' THEN TRY_CAST(regexp_extract(info, '^([0-9]+)''', 1) AS DOUBLE) * 30.48 +
        COALESCE(TRY_CAST(regexp_extract(info, '''\s*([0-9]+)', 1) AS DOUBLE),
                                                                                                                                                     0) * 2.54 ELSE NULL END AS height_cm
FROM RAW )
SELECT count(*),
       count(height_cm) AS parsed_ok,
       min(height_cm),
       max(height_cm)
FROM parsed;

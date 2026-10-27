SELECT cn.name,
       mc.note
FROM movie_companies mc
JOIN company_name cn ON cn.id = mc.company_id
JOIN company_type ct ON ct.id = mc.company_type_id
WHERE mc.movie_id = 2392833
  AND ct.kind = 'production companies';

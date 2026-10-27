SELECT count(DISTINCT mc.movie_id)
FROM movie_companies mc
JOIN company_type ct ON ct.id=mc.company_type_id
WHERE mc.company_id=1310
  AND ct.kind='production companies';

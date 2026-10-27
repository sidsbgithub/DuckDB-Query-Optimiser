SELECT n.name, t.title, rt.role, t.production_year, ci.nr_order
FROM cast_info ci
JOIN title t ON t.id = ci.movie_id
JOIN kind_type kt ON kt.id = t.kind_id
JOIN name n ON n.id = ci.person_id
JOIN role_type rt ON rt.id = ci.role_id
LEFT JOIN (SELECT pib.person_id, min(pib.info) AS bio
           FROM person_info pib JOIN info_type itb ON itb.id = pib.info_type_id
           WHERE itb.info = 'mini biography' GROUP BY pib.person_id) opt_b
    ON opt_b.person_id = n.id
LEFT JOIN (SELECT pid.person_id, min(pid.info) AS birth_date
           FROM person_info pid JOIN info_type itd ON itd.id = pid.info_type_id
           WHERE itd.info = 'birth date' GROUP BY pid.person_id) opt_d
    ON opt_d.person_id = n.id
WHERE t.production_year BETWEEN 1998 AND 2003
  AND kt.kind IN ('episode', 'movie', 'video movie', 'tv movie', 'tv series', 'video game')
  AND EXISTS (SELECT 1 FROM movie_companies mcf JOIN company_name cnf ON cnf.id = mcf.company_id WHERE mcf.movie_id = t.id AND cnf.country_code = '[ca]')
ORDER BY rt.role DESC, ci.id
LIMIT 100 OFFSET 0;

SELECT country_code,
       count(*)
FROM company_name
GROUP BY country_code
ORDER BY count(*) DESC
LIMIT 20;

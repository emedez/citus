--
-- queries to test the subquery pushdown on reference tables

-- subqueries in WHERE with greater operator
SELECT 
  user_id
FROM 
  users_table
WHERE 
  value_2 >  
          (SELECT 
              max(value_2) 
           FROM 
              events_reference_table  
           WHERE 
              users_table.user_id = events_reference_table.user_id AND event_type = 50
           GROUP BY
              user_id
          )
GROUP BY user_id
HAVING count(*) > 66
ORDER BY user_id
LIMIT 5;

-- subqueries in WHERE with IN operator
SELECT 
  user_id
FROM 
  users_table
WHERE 
  value_2 IN  
          (SELECT 
              value_2 
           FROM 
              events_reference_table  
           WHERE 
              users_table.user_id = events_reference_table.user_id
          )
GROUP BY user_id
ORDER BY user_id
LIMIT 3;

-- subqueries in WHERE with NOT EXISTS operator, should work since
-- reference table in the inner part of the join
SELECT 
  user_id
FROM 
  users_table
WHERE 
  NOT EXISTS  
      (SELECT 
          value_2 
       FROM 
          events_reference_table  
       WHERE 
          users_table.user_id = events_reference_table.user_id
      )
GROUP BY user_id
ORDER BY user_id
LIMIT 3;

-- subqueries in WHERE with NOT EXISTS operator, should not work
-- there is a reference table in the outer part of the join
SELECT 
  user_id
FROM 
  users_reference_table
WHERE 
  NOT EXISTS  
      (SELECT 
          value_2 
       FROM 
          events_table  
       WHERE 
          users_reference_table.user_id = events_table.user_id
      )
LIMIT 3;

-- subqueries in WHERE with IN operator without equality
SELECT 
  user_id
FROM 
  users_table
WHERE 
  value_2 IN  
          (SELECT 
              value_2 
           FROM 
              events_reference_table  
           WHERE 
              users_table.user_id > events_reference_table.user_id
          )
GROUP BY user_id
ORDER BY user_id
LIMIT 3;

-- have reference table without any equality, should error out 
SELECT 
  user_id
FROM 
  users_table
WHERE 
  value_2 >  
          (SELECT 
              max(value_2) 
           FROM 
              events_reference_table  
           WHERE 
              event_type = 50
           GROUP BY
              user_id
          )
GROUP BY user_id
HAVING count(*) > 66
ORDER BY user_id
LIMIT 5; 

-- users that appeared more than 118 times, should run since the reference table
-- on the right side of the semi join
SELECT 
  user_id
FROM 
  users_table
WHERE 118 <=
      (SELECT 
          count(*) 
        FROM 
          events_reference_table 
        WHERE 
        	users_table.user_id = events_reference_table.user_id 
        GROUP BY 
        	user_id)
GROUP BY 
	user_id
ORDER BY
	user_id;

-- should error out since reference table exist on the left side 
-- of the left lateral join
SELECT user_id, value_2 FROM users_table WHERE
  value_1 > 101 AND value_1 < 110
  AND value_2 >= 5
  AND user_id IN
  (
		SELECT
		  e1.user_id
		FROM (
		  -- Get the first time each user viewed the homepage.
		  SELECT
		    user_id,
		    1 AS view_homepage,
		    min(time) AS view_homepage_time
		  FROM events_reference_table
		     WHERE
		     event_type IN (10, 20, 30, 40, 50, 60, 70, 80, 90)
		  GROUP BY user_id
		) e1 LEFT JOIN LATERAL (
		  SELECT
		    user_id,
		    1 AS use_demo,
		    time AS use_demo_time
		  FROM events_reference_table
		  WHERE
		    user_id = e1.user_id AND
		       event_type IN (11, 21, 31, 41, 51, 61, 71, 81, 91)
		  ORDER BY time
		) e2 ON true LEFT JOIN LATERAL (
		  SELECT
		    user_id,
		    1 AS enter_credit_card,
		    time AS enter_credit_card_time
		  FROM  events_reference_table
		  WHERE
		    user_id = e2.user_id AND
		    event_type IN (12, 22, 32, 42, 52, 62, 72, 82, 92)
		  ORDER BY time
		) e3 ON true LEFT JOIN LATERAL (
		  SELECT
		    1 AS submit_card_info,
		    user_id,
		    time AS enter_credit_card_time
		  FROM  events_reference_table
		  WHERE
		    user_id = e3.user_id AND
		    event_type IN (13, 23, 33, 43, 53, 63, 73, 83, 93)
		  ORDER BY time
		) e4 ON true LEFT JOIN LATERAL (
		  SELECT
		    1 AS see_bought_screen
		  FROM  events_reference_table
		  WHERE
		    user_id = e4.user_id AND
		    event_type IN (14, 24, 34, 44, 54, 64, 74, 84, 94)
		  ORDER BY time
		) e5 ON true
		group by e1.user_id
		HAVING sum(submit_card_info) > 0
)
ORDER BY 1, 2;

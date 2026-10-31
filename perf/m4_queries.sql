-- the twenty query workload of REPORT_M4.md; multi-way joins use JOIN ON so the unoptimised plan
-- (joins in written order, all nested loops, nothing pushed) is a join, not a cross product
SELECT name, age FROM Emp WHERE age > 60;
SELECT COUNT(*) FROM Emp WHERE did = 7;
SELECT COUNT(*) FROM Emp WHERE level = 8 AND salary > 160000;
SELECT e.name, d.title FROM Emp e JOIN Dept d ON e.did = d.did WHERE d.budget > 50000;
SELECT e.name FROM Emp e, Dept d WHERE e.did = d.did AND e.age > 60 AND d.budget > 1000;
SELECT did, COUNT(*) AS n, AVG(salary) AS avg_sal FROM Emp WHERE age > 25 GROUP BY did HAVING COUNT(*) > 5 ORDER BY n DESC, did LIMIT 10;
SELECT e.name, m.name FROM Emp e JOIN Emp m ON e.mgr = m.eid WHERE e.age < 22;
SELECT t.did, t.n FROM (SELECT did, COUNT(*) AS n FROM Emp GROUP BY did) AS t WHERE t.n > 40;
SELECT e.name, p.name, w.hours FROM Emp e JOIN Works w ON e.eid = w.eid JOIN Proj p ON w.pid = p.pid JOIN Dept d ON p.did = d.did WHERE d.city = 'Ottawa' AND w.hours > 35;
SELECT p.pid, p.name, COUNT(*) AS staff FROM Proj p JOIN Works w ON p.pid = w.pid JOIN Emp e ON w.eid = e.eid WHERE e.city = 'Toronto' GROUP BY p.pid, p.name HAVING COUNT(*) > 2 ORDER BY staff DESC, p.pid LIMIT 5;
SELECT w.pid, SUM(w.hours) AS total FROM Works w GROUP BY w.pid HAVING SUM(w.hours) > 400 ORDER BY total DESC, w.pid;
SELECT COUNT(*) FROM Works w JOIN Proj p ON w.pid = p.pid WHERE p.pid = 7;
SELECT e.level, d.city, COUNT(*) FROM Emp e JOIN Dept d ON e.did = d.did GROUP BY e.level, d.city ORDER BY e.level, d.city;
SELECT DISTINCT e.city, d.city FROM Emp e JOIN Dept d ON e.did = d.did;
SELECT e.name FROM Emp e JOIN Dept d ON e.did = d.did AND e.city = d.city WHERE e.age = 30;
SELECT d.title, COUNT(*) FROM Dept d JOIN Proj p ON d.did = p.did WHERE p.budget < 100000 GROUP BY d.title;
SELECT e.name, w.hours FROM Emp e JOIN Works w ON e.eid = w.eid WHERE e.level = 8 AND e.salary > 160000;
SELECT city FROM Emp WHERE level = 1 UNION SELECT city FROM Dept WHERE budget > 90000;
SELECT e.eid FROM Emp e JOIN Works w ON e.eid = w.eid JOIN Proj p ON w.pid = p.pid WHERE p.budget > 450000 AND e.age > 50;
SELECT COUNT(*) FROM Emp e JOIN Works w ON e.eid = w.eid WHERE w.hours < 3 AND e.level < 3;

console.log("JavaScript, on an OS that wrote its own kernel.");
let sum = 0;
for (let i = 1; i <= 100; i++) sum += i;
console.log("sum 1..100 =", sum);
const xs = [5,3,9,1].sort((a,b) => a-b).map(x => x * x);
console.log("sorted+squared:", JSON.stringify(xs));
console.log("regexp:", "quickjs-2024".match(/[a-z]+-(\d+)/)[1]);
console.log("closure:", (f => f(f, 10))((f, n) => n <= 1 ? 1 : n * f(f, n-1)));

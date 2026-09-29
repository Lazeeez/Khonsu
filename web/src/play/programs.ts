// Preset Lune programs for the Play page. Each has a `seed` so speculation has a knob.

export interface Preset {
  name: string;
  blurb: string;
  knob: string; // the variable speculative branches vary
  src: string;
}

export const PRESETS: Preset[] = [
  {
    name: "Ledger",
    blurb: "4,096 accounts and a 2 MiB journal (512 pages). Fork mid-run: the child shares the journal it inherited and copies only what it writes next.",
    knob: "seed",
    src: `# 4,096 accounts, and a journal with one entry per transfer (2 MiB).
let seed = 1
let accounts = array(4096)
let journal = array(262144)
let i = 0
while i < len(accounts) { accounts[i] = 100; i = i + 1 }
print "opened"

let x = seed
let t = 0
while t < len(journal) {
  x = hash(x)
  let from = x % len(accounts)
  x = hash(x)
  let to = x % len(accounts)
  let amount = x % 50 + 1
  if accounts[from] >= amount {
    accounts[from] = accounts[from] - amount
    accounts[to] = accounts[to] + amount
    journal[t] = amount
  }
  t = t + 1
  if t % 32768 == 0 { print t }
}

let richest = 0
i = 0
while i < len(accounts) {
  if accounts[i] > richest { richest = accounts[i] }
  i = i + 1
}
print "richest account holds"
print richest
`,
  },
  {
    name: "Monte Carlo π",
    blurb: "Estimates π from hashed points. Speculate over seeds and watch the estimates disagree, deterministically.",
    knob: "seed",
    src: `# π by Monte Carlo; prints π × 1,000,000 as the estimate improves.
let seed = 42
let inside = 0
let n = 0
let x = seed
while n < 400000 {
  x = hash(x)
  let px = x % 1000000
  x = hash(x)
  let py = x % 1000000
  if px * px + py * py < 1000000000000 { inside = inside + 1 }
  n = n + 1
  if n % 50000 == 0 { print inside * 4000000 / n }
}
`,
  },
  {
    name: "Gambler",
    blurb: "A gambler bets until broke or rich. Which seeds survive? Speculate 16 futures from one moment.",
    knob: "seed",
    src: `# Gambler's ruin with a 49% edge against the house.
let seed = 7
let money = 100
let x = seed
let rounds = 0
while money > 0 and money < 300 and rounds < 100000 {
  x = hash(x)
  if x % 100 < 49 { money = money + 10 } else { money = money - 10 }
  rounds = rounds + 1
  if rounds % 50 == 0 { print money }
}
if money >= 300 { print "rich after rounds" } else { print "broke after rounds" }
print rounds
`,
  },
  {
    name: "Sieve",
    blurb: "Primes below 1,000,000 (8 MiB of state). Fork at any instruction; both halves finish identically.",
    knob: "n",
    src: `# Sieve of Eratosthenes.
let n = 1000000
let composite = array(n)
let i = 2
let count = 0
while i < n {
  if composite[i] == 0 {
    count = count + 1
    let j = i * i
    while j < n { composite[j] = 1; j = j + i }
  }
  i = i + 1
}
print "primes below n"
print count
`,
  },
];

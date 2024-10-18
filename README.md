# Linear Decision Diagrams for Workload Representation

Summary: This is Luc Edes (URF under Professor Mina Winter 2024). While working on fperf, Professor Mina, Amir and I frequently found that the current Workload representation was quite limiting in how we could express and manipulate them.\
Examples:
- We could not easily express the concept of a workload that is a union of two other workloads (disjunction).
- `aipg` was an unelegant metric for expressing gaps in enqueues.
- There were some others I'm now forgetting (note that I'm working on this in the Spring, after the end of my URF)

Our idea was to use [Linear Decision Diagrams](https://ieeexplore.ieee.org/document/5351143), a variant of [Binary Decision Diagrams](https://en.wikipedia.org/wiki/Binary_decision_diagram) derived from [Difference Decision Diagrams](https://link.springer.com/content/pdf/10.1007/3-540-48168-0_9.pdf).\
They provide:
- A compact representation of linear arithmetic constraints (note that Workloads are simply constraints on enq variables)
- Dynamic Variable Ordering: Optimize size of decision diagrams, reduce complexity
- Local Reductions

The authors (including Arie Gurfinkel, currently at UW) provide an [open-source implementation](https://github.com/seahorn/ldd) in C++ based on the [CUDD package](https://github.com/ivmai/cudd).

Using a linear arithmetic theory, each variable would represent `enq[q][t]` for some queue `q` and time `t`.\
Thus, a `cenq` constraint would be represented as a linear inequality.\
Example: `[1, 3]: cenq(2, t) >= t` could be represented using the following 3 inequalities:
- `enq[2][1] >= 1`
- `enq[2][1] + enq[2][2] >= 2`
- `enq[2][1] + enq[2][2] + enq[2][3] >= 3`

See the RR test in tests.cpp for some initial tests of how this could be used for representing Workloads.\








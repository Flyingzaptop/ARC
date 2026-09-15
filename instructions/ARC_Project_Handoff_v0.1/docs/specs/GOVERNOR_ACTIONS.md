# Governor action capability table

Every action has a capability requirement.

| Action | Earliest stage | Risk | Reversible | Notes |
|---|---:|---|---|---|
| Observe | 1 | Very low | n/a | Must not alter output |
| Prefetch | 2 | Low | Yes | Costs bandwidth/memory |
| Evict cold safe resource | 2 | Low-Med | Yes | Must respect sync |
| Keep RAM warm cache | 2 | Low | Yes | Uses system RAM |
| Remove top texture mip residency | 3 | Medium | Yes | Only safe texture classes |
| Promote texture mip | 3 | Low | Yes | Requires transfer |
| Change shadow resource quality | 5 | Medium-High | Usually | Integration-specific |
| Change render scale | 5/6 | Medium | Yes | Requires compatible integration |
| Enable upscaling | 6 | Medium | Yes | Needs correct inputs |
| Enable frame generation | 6 | Medium | Yes | Needs temporal data |
| Modify unknown compute resource | Never automatic | Extreme | Unknown | Forbidden |

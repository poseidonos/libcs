# Scenario Complexity Metrics

## Original Scenarios

| Scenario | Total lines | API calls | Error checks | PAIN annotations | Boilerplate ratio | Notes |
|---|---:|---:|---:|---:|---:|---|
| scenario_01 | 80 | 9 | 5 | 2 | 0.56 | Minimal working flow still needs explicit lifecycle management |
| scenario_02 | 190 | 39 | 11 | 3 | 0.28 | Memory + copy path adds significant call volume |
| scenario_03 | 104 | 9 | 5 | 3 | 0.56 | Batch setup is concise in call count but descriptor-heavy |
| scenario_04 | 163 | 8 | 10 | 2 | 1.25 | Production cleanup and async checks dominate control flow |

## Improved Scenarios

| Scenario | Total lines | API calls | Error checks | Boilerplate ratio | vs. original |
|---|---:|---:|---:|---:|---|
| scenario_01_improved | 34 | 1 | 1 | 1.00 | API calls -8, lines -46 |
| scenario_02_improved | 59 | 7 | 2 | 0.29 | API calls -32, lines -131 |
| scenario_03_improved | 67 | 4 | 2 | 0.50 | API calls -5, lines -37 |
| scenario_04_improved | 54 | 8 | 2 | 0.25 | API calls 0, lines -109, error checks -8 |
| **Average** | - | - | - | **Before: 0.66 / After: 0.51** | **22.64% lower average boilerplate ratio** |

## Counting Rules
- API calls: every `cs*()` invocation in source.
- Error checks: any `if (... != CS_STATUS_SUCCESS)` guard (including related status variables).
- PAIN annotations: every `PAIN[...]` structured comment.

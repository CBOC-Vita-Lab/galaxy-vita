import re
import sys

ir = open(sys.argv[1], encoding="utf-8").read()
functions = re.findall(r"^define[^\n]*?@([^ (]+)[^\n]*\{(.*?)^\}", ir, re.M | re.S)
for function, operation in (("SetWarmBudget", "store"), ("read_warm_budget_probe", "load"),
                            ("RunWarm", "load"), ("WarmIdle", "load")):
    bodies = [body for name, body in functions if function in name]
    assert len(bodies) == 1, "expected one compiled function: " + function
    assert re.search(r"\b" + operation + r" atomic[^\n]*g_warmBudgetMs", bodies[0]), \
        function + " must access the warm budget atomically"
print("PASS: compiled real warm-budget publication/consumption use atomic accesses")

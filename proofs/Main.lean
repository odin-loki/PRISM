import Prism.Export

/-- Prints the verdict truth tables (testdata/fixtures/verdict_tables.json). -/
def main : IO Unit := IO.print Prism.Export.tables

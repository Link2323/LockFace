#pragma once

// Every live COM instance (including a credential retained during dispatch)
// keeps its code and window procedure loaded until owner-thread teardown ends.
void LockFaceDllAddRef();
void LockFaceDllRelease();

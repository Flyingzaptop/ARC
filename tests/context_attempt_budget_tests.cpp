#include "arc/cpu/context_attempt_budget.hpp"
#include <limits>
#include <initializer_list>
int main(){
 arc::cpu::ContextAttemptBudget failed_single;
 if(failed_single.account(5001)||failed_single.enabled||failed_single.total_us!=5001)return 1;
 arc::cpu::ContextAttemptBudget failed_many;
 for(int i=0;i<50;++i)if(!failed_many.account(1000))return 2;
 if(failed_many.account(1)||failed_many.enabled||failed_many.total_us!=50001)return 3;
 arc::cpu::ContextAttemptBudget mixed;
 // Account is independent of capture success: failures and successes have the same cost path.
 for(bool success:{false,true,false,true}){(void)success;if(!mixed.account(2500))return 4;}
 if(mixed.total_us!=10000)return 5;
 if(mixed.account(std::numeric_limits<double>::infinity()))return 6;
 return 0;
}

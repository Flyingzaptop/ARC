"""Independent admission obligations for a CPU replacement.

This executable contract checker consumes facts from a TRUSTED verifier/provider.
JSON, differential tests, observations and the caller's assertions are not such a
provider. No provider or live dispatcher is installed by this module. The two axes
remain independent even when one is fully established.
"""
from dataclasses import dataclass

@dataclass(frozen=True)
class Semantics:
    code_generation_matches: bool = False
    path_input_guards_pass: bool = False
    exact_permutation_all_paths: bool = False
    continuation_state_preserved: bool = False
    exception_and_external_effects_preserved: bool = False

@dataclass(frozen=True)
class Access:
    allocation_id: int = 0
    generation: int = 0
    begin: int = 0
    size: int = 0
    # Proof/enforcement must cover reads AND writes, free/remap, new threads,
    # escaped aliases, kernel/driver/DMA access; a sampled thread set is not enough.
    complete_access_domain: bool = False
    exclusive_through_commit: bool = False
    lifetime_pinned: bool = False
    consumer_publication_held: bool = False
    provider: str = 'observation_only'
    epoch: int = 0

@dataclass(frozen=True)
class Ticket:
    allocation_id: int
    generation: int
    begin: int
    size: int
    epoch: int

def check(semantics,access):
    semantic_failures=[field for field in semantics.__dataclass_fields__ if getattr(semantics,field) is not True]
    memory_failures=[]
    if access.provider not in ('verified_closed_alias_domain','enforced_complete_access_lease'):
        memory_failures.append('no_verified_ownership_provider')
    for field in ('complete_access_domain','exclusive_through_commit','lifetime_pinned','consumer_publication_held'):
        if getattr(access,field) is not True:memory_failures.append(field)
    if not (access.allocation_id>0 and access.generation>0 and access.epoch>0):
        memory_failures.append('missing_allocation_generation_or_epoch')
    if not (access.begin>0 and access.begin%16==0 and 0<access.size<=4*1024*1024 and access.size%16==0
            and access.begin+access.size<1<<64):memory_failures.append('invalid_range')
    return {'semantics_pass':not semantic_failures,'memory_pass':not memory_failures,
            'eligible':not semantic_failures and not memory_failures,
            'semantic_failures':semantic_failures,'memory_failures':memory_failures}

def before_dispatch(semantics,access):
    result=check(semantics,access)
    ticket=Ticket(access.allocation_id,access.generation,access.begin,access.size,access.epoch) if result['eligible'] else None
    return result,ticket

def before_commit(semantics,access,ticket,*,gpu_complete,scratch_verified):
    result=check(semantics,access)
    failures=[]
    if ticket is None:failures.append('no_dispatch_ticket')
    elif ticket!=Ticket(access.allocation_id,access.generation,access.begin,access.size,access.epoch):
        failures.append('lease_or_allocation_changed')
    if not gpu_complete:failures.append('gpu_not_complete')
    if not scratch_verified:failures.append('scratch_not_verified')
    result['commit_failures']=failures;result['eligible']=result['eligible'] and not failures
    # A real provider must HOLD exclusion across this check and the entire write;
    # checking twice without holding the lease leaves a TOCTOU race.
    return result

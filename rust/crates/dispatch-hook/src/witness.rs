use dispatch_case::ped_kind_from_type;
use dispatch_core::PedKind;

/// Peds that can perceive crimes and feed the witness / report pipeline.
pub fn witness_ped_type_eligible(ped_type: i32) -> bool {
    matches!(
        ped_kind_from_type(ped_type),
        PedKind::Civilian | PedKind::Cop
    )
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn civilians_and_cops_are_witness_eligible() {
        assert!(witness_ped_type_eligible(4));
        assert!(witness_ped_type_eligible(5));
        assert!(witness_ped_type_eligible(6));
        assert!(!witness_ped_type_eligible(0));
    }
}
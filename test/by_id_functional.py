import uuid


async def test_get_capture_by_id_not_found(service_client):
    random_id = uuid.uuid4()
    response = await service_client.get(f"/v1/capture/{random_id}")

    assert response.status == 404
    body = response.json()
    assert body["error"]["message"] == "capture not found"


<script>
    import {onMount} from "svelte";

    let fetchPromise = (async function() {return await (await fetch("/scripts")).json()})();
    let scripts = {};
    onMount(async () => {
        scripts = await fetchPromise;
    })
    async function activate(script) {
        await fetch("/scripts/" + encodeURIComponent(script) + "/activate", {method: "POST"})
        scripts[script].active = !scripts[script].active;
    }

    async function deactivate(script) {
        await fetch("/scripts/" + encodeURIComponent(script) + "/deactivate", {method: "POST"})
        scripts[script].active = !scripts[script].active;
    }
    async function deleteScript(script) {
        if(!confirm("Are you sure you want to delete " + script + "?")) {
            return;
        }
        let resp = await fetch("/scripts/" + script, {method: "DELETE"});
        if(resp.status !== 200)
            return;
        delete scripts[script]
        scripts = scripts
    }
    async function newScript() {
        let script = prompt("Please enter the name of the new script");
        if(script === null)
            return;
        let resp = await fetch("/scripts/" + script, {method: "PUT"});
        if(resp.status !== 200)
            return;
        scripts[script] = {name: script, active: true}
        // sort
        scripts = Object.keys(scripts).sort().reduce(
            (obj, key) => {
                obj[key] = scripts[key];
                return obj;
            },
            {}
        );
        console.log(scripts)
    }
</script>

<main>
    {#await fetchPromise}
    {:then}
        <div id="toolbar">

            <span class="toolbar-item" id="newScript" on:click={newScript}>New</span>
        </div>
        <div id="scripts">
        {#each Object.keys(scripts) as script (script)}
            <div class="script">
                <div class="scriptName">
                    {scripts[script].name}
                </div>
                {#if scripts[script].active}
                    <button on:click={deactivate(script)}>
                        Deactivate
                    </button>
                {:else}
                    <button on:click={activate(script)}>
                        Activate
                    </button>
                {/if}
                <button onclick="window.location = 'edit.html?script={script}'">
                    Edit
                </button>
                <button on:click={deleteScript(script)}>
                    Delete
                </button>
            </div>
        {/each}
        </div>
    {:catch error}
        <div id="scripts">
            {error.message}
        </div>
    {/await}
</main>

<style>
.script {
    background-color: white;
    border-radius: 10px;
    padding: 10px;
    box-shadow: #9f9f9f 0 0 5px;
    margin-bottom: 20px;
    display: grid;
    grid-template-columns: auto min-content min-content min-content;
    align-items: center;
}
main {
    font-size: var(--medium-font-size);
}
#toolbar {
    margin-bottom: 20px;
    display: flex;
    align-items: center;
}
.toolbar-item {
    border: 2px solid #8be4ff;
    background-color: white;
    box-shadow: #9f9f9f 0 0 5px;
    border-radius: 10px;
    padding: 10px;
    cursor: pointer;

}
.script > * {
    margin: 0 0 0 10px;
}
</style>
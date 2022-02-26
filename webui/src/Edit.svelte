
<script>
    import {EditorView, keymap} from "@codemirror/view"
    import {basicSetup} from "@codemirror/basic-setup"
    import {EditorState} from "@codemirror/state"
    import {indentUnit} from "@codemirror/language"
    import {javascript} from "@codemirror/lang-javascript"
    import {indentWithTab} from "@codemirror/commands"
    import {onMount} from "svelte";

    let myView;
    let file = new URLSearchParams(window.location.search).get("script");
    onMount(async () => {
        myView = new EditorView({
            state: EditorState.create({extensions: [basicSetup, javascript(), indentUnit.of("    "),  keymap.of([indentWithTab])]}),
        })
        let resp = await fetch("/scripts/" + file);
        if(resp.status !== 200) {
            console.error(await resp.text());
            return;
        }
        let contentJson = await resp.json();
        let code = contentJson.code;
        myView.dispatch({
            changes: {from: 0, to: myView.state.doc.length, insert: code}
        })

        document.getElementById("editor").appendChild(myView.dom);
    })
    async function saveScript() {
        try {
            await fetch("/scripts/" + file, {method: "PUT", body: myView.state.doc.toString()});
        } catch(e) {
            alert("Failed to upload: " + e);
        }
    }
    document.addEventListener('keydown', e => {
        if (e.ctrlKey && e.key === 's') {
            e.preventDefault();
            saveScript();
        }
    });
</script>


<main>
    <span id="toolbar">
        <button on:click={saveScript}>Save</button>
        <span>{file}</span>
    </span>

    <div id="editor"></div>
</main>
<style>
    #toolbar {
        display: flex;
        align-items: center;
    }
    #editor {
        font-size: var(--small-font-size);
        margin-bottom: 100px;
        word-break: break-word;
    }
    main {
        background-color: white;
        box-shadow: #333333 0 0 5px;
        border-radius: 10px;
    }
    #toolbar > * {
        margin: 5px;
    }
</style>